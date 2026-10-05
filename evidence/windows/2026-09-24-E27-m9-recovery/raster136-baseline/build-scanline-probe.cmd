@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cl /nologo /MT /O2 /W4 /WX P:\bc-250\bc250-win\experiments\E27-m9-inference\scanline_probe.c /Fe:P:\bc-250\scratch\m13\scanline_probe.exe /Fo:P:\bc-250\scratch\m13\scanline_probe.obj /link gdi32.lib user32.lib
