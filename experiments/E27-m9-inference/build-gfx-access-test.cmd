@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\gfx-access-test mkdir %BC250_ROOT%\scratch\m9\gfx-access-test
cd /d %BC250_ROOT%\scratch\m9\gfx-access-test
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-gfx-access-test.py %BC250_ROOT%\bc250-win\driver\kmd\gfx.c undrained.c --disable-drain
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC undrained.c /Fe:undrained.exe
if errorlevel 1 exit /b 1
undrained.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-gfx-access-test.py %BC250_ROOT%\bc250-win\driver\kmd\gfx.c after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC after.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
