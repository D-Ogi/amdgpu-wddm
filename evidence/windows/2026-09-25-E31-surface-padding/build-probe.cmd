@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cl /nologo /MT /O2 /EHsc P:\bc-250\scratch\m10\surface-padding\render_probe_padding.cpp /Fe:P:\bc-250\scratch\m10\surface-padding\render_probe_padding.exe /Fo:P:\bc-250\scratch\m10\surface-padding\render_probe_padding.obj /link /SUBSYSTEM:WINDOWS
