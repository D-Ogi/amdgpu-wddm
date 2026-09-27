@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cd /d P:\bc-250\scratch\fl-probe-2026-09-27
cl /nologo /W4 /O2 /MT /Fo:fl-probe.obj fl-probe.c /link /OUT:fl-probe.exe d3d12.lib dxgi.lib
exit /b %errorlevel%
