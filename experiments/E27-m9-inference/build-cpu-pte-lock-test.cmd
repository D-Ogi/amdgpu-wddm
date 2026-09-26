@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\cpu-pte-lock-test mkdir %BC250_ROOT%\scratch\m9\cpu-pte-lock-test
cd /d %BC250_ROOT%\scratch\m9\cpu-pte-lock-test
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-cpu-pte-lock-test.py %BC250_ROOT%\bc250-win\driver\kmd\vidmm.c unlocked.c --unlocked
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC unlocked.c /Fe:unlocked.exe
if errorlevel 1 exit /b 1
unlocked.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-cpu-pte-lock-test.py %BC250_ROOT%\bc250-win\driver\kmd\vidmm.c reader-unlocked.c --reader-unlocked
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC reader-unlocked.c /Fe:reader-unlocked.exe
if errorlevel 1 exit /b 1
reader-unlocked.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-cpu-pte-lock-test.py %BC250_ROOT%\bc250-win\driver\kmd\vidmm.c after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC after.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
