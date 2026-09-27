@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cd /d P:\bc-250\scratch\g0-hosted\map-lifetime003
cl /nologo /std:c11 /O2 /W4 /WX /I P:\bc-250\scratch\g0-umd-audit-source\src /I P:/bc-250/scratch/g0-umd-audit-source/include control.c /Fe:control.exe /Fo:control.obj
if errorlevel 1 exit /b 1
control.exe 1>control.stdout.log 2>control.events.log
exit /b %errorlevel%
