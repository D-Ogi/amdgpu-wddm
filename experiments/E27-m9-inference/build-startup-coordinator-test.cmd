@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\startup-coordinator mkdir %BC250_ROOT%\scratch\m9\startup-coordinator
cd /d %BC250_ROOT%\scratch\m9\startup-coordinator
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-startup-coordinator-test.py %BC250_ROOT%\bc250-win test.c %*
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /I%BC250_ROOT%\bc250-win\driver\kmd test.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
