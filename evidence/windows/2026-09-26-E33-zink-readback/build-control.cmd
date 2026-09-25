@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cl /nologo /O2 /W4 /MT P:\bc-250\scratch\m12\wgl-resize-control.c /FoP:\bc-250\scratch\m12\wgl-resize-control.obj /FeP:\bc-250\scratch\m12\wgl-resize-control.exe /link opengl32.lib gdi32.lib user32.lib
