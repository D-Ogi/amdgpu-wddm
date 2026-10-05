@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted
cl /nologo /EHsc /O2 /MT runtime-wrap-clamp-control.cpp /Fe:runtime-wrap-clamp-control.exe /link dbghelp.lib d3dcompiler.lib d3d11.lib dxgi.lib user32.lib
