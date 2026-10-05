@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\post-cache-test mkdir %BC250_ROOT%\scratch\m9\post-cache-test
cd /d %BC250_ROOT%\scratch\m9\post-cache-test
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py %BC250_ROOT%\bc250-win mutation.c --always-nc
cl /nologo /W4 /WX /TC mutation.c /Fe:mutation.exe
if errorlevel 1 exit /b 1
mutation.exe
if not errorlevel 1 exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-post-cache-test.py %BC250_ROOT%\bc250-win actual.c
cl /nologo /W4 /WX /TC actual.c /Fe:actual.exe
if errorlevel 1 exit /b 1
actual.exe
