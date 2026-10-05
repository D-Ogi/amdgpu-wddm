@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\paging-lifetime-test mkdir P:\bc-250\scratch\m9\paging-lifetime-test
cd /d P:\bc-250\scratch\m9\paging-lifetime-test
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-paging-lifetime-test.py P:\bc-250\bc250-win\driver\kmd\gfx.c unlocked.c --disable-builder-lock
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC unlocked.c /Fe:unlocked.exe
if errorlevel 1 exit /b 1
unlocked.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-paging-lifetime-test.py P:\bc-250\bc250-win\driver\kmd\gfx.c after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC after.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
