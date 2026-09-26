@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cd /d P:\bc-250\scratch\m13\runtime-probe
cl /nologo /EHsc /O2 /MT runtime-control.cpp /Fe:runtime-control.exe /link d3d11.lib dxgi.lib user32.lib
