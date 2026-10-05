@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
cd /d %BC250_ROOT%\scratch\m13\native-shared
cl /nologo /EHsc /O2 /MT native-control.cpp /Fe:native-control.exe /link gdi32.lib d3d11.lib dxgi.lib user32.lib
