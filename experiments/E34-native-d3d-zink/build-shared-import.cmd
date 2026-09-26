@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cd /d P:\bc-250\scratch\m13\shared-import
cl /nologo /EHsc /O2 /MT /I P:\bc-250\ref\Vulkan-Headers\include probe.cpp /Fe:shared-import.exe /link gdi32.lib dxgi.lib user32.lib
