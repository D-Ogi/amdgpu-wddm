@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
cl /nologo /MT /O2 /EHsc %BC250_ROOT%\bc250-win\experiments\E26-wddm-desktop\render_probe.cpp /Fe:%BC250_ROOT%\scratch\dwm\render_probe.exe /Fo:%BC250_ROOT%\scratch\dwm\render_probe.obj /link /SUBSYSTEM:WINDOWS
