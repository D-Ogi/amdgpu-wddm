@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\pte-copy-storage-test mkdir %BC250_ROOT%\scratch\m9\pte-copy-storage-test
cd /d %BC250_ROOT%\scratch\m9\pte-copy-storage-test
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-pte-copy-storage-test.py %BC250_ROOT%\bc250-win\driver\kmd\gfx.c actual.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC actual.c /Fe:actual.exe
if errorlevel 1 exit /b 1
actual.exe
