@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted
cl /nologo /EHsc /W4 /WX /O2 /MT P:\BC-250\bc250-win\experiments\E34-native-d3d-zink\hosted-runtime\graphics-state-control.cpp /Fe:graphics-texture-control.exe /link d3d11.lib dxgi.lib d3dcompiler.lib
