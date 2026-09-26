@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\init-entrypoints mkdir %BC250_ROOT%\scratch\m9\init-entrypoints
cd /d %BC250_ROOT%\scratch\m9\init-entrypoints
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-init-entrypoints-test.py %BC250_ROOT%\bc250-win init.c %BC250_ROOT%\scratch\m9\init-entrypoints-before
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /I%BC250_ROOT%\bc250-win\driver\kmd init.c /Fe:init.exe
if errorlevel 1 exit /b 1
init.exe

if errorlevel 1 exit /b 1
python %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\generate-init-entrypoints-test.py %BC250_ROOT%\bc250-win mutation.c %BC250_ROOT%\scratch\m9\init-entrypoints-before --omit-completion-check
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /I%BC250_ROOT%\bc250-win\driver\kmd mutation.c /Fe:mutation.exe
if errorlevel 1 exit /b 1
mutation.exe
if not "%ERRORLEVEL%"=="1" exit /b 1
exit /b 0
