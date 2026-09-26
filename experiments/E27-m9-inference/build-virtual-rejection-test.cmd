@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\virtual-rejection-test mkdir %BC250_ROOT%\scratch\m9\virtual-rejection-test
cd /d %BC250_ROOT%\scratch\m9\virtual-rejection-test
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-virtual-rejection-test.py %BC250_ROOT%\scratch\m9\rejection-before\wddm.c before.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /wd4189 /TC /I%BC250_ROOT%\bc250-win\driver\kmd before.c %BC250_ROOT%\bc250-win\driver\kmd\umd_blob.c /Fe:before.exe
if errorlevel 1 exit /b 1
before.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-virtual-rejection-test.py %BC250_ROOT%\bc250-win\driver\kmd\wddm.c after.c
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /I%BC250_ROOT%\bc250-win\driver\kmd after.c %BC250_ROOT%\bc250-win\driver\kmd\umd_blob.c /Fe:after.exe
if errorlevel 1 exit /b 1
after.exe
