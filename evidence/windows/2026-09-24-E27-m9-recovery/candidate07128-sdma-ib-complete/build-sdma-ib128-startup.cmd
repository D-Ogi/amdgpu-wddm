@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\build\sdma-ib128-startup mkdir P:\bc-250\scratch\build\sdma-ib128-startup
cd /d P:\bc-250\scratch\build\sdma-ib128-startup
python P:\bc-250\bc250-win\driver\kmd\test\generate_cp_startup_test.py test.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC test.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
