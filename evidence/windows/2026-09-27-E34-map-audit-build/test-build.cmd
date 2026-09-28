@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted\map-audit002
cl /nologo /std:c11 /O2 /W4 /WX control.c /Fe:control.exe
if errorlevel 1 exit /b 1
control.exe
