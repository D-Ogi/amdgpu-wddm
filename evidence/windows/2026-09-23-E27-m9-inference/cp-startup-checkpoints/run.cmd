@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\cp-startup
cl /nologo /W4 /WX /O2 /std:c11 test.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
