@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted\dwm035
set INCLUDE=P:\BC-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;P:\BC-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
cl /nologo /O2 /MD /W4 /WX /LD router.cpp /Fe:router.dll

if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX composition-control.cpp /Fe:composition-control.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib dwmapi.lib

if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /W4 /WX test-shared-log.cpp /Fe:test-shared-log.exe

if errorlevel 1 exit /b 1
copy /y P:\bc-250\bc250-win\experiments\E34-native-d3d-zink\hosted-runtime\dwm031-gpu-present\shared-log.h shared-log-old.h >nul
cl /nologo /O2 /MD /W4 /WX test-detached-log.cpp /Fe:test-detached-log.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
if errorlevel 1 exit /b 1
cl /nologo /O2 /MD /W4 /WX /DTEST_OLD_REDIRECT test-detached-log.cpp /Fe:test-detached-log-old.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
